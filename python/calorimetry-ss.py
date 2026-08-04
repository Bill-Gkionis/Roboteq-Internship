import yaml
with open('params.yaml', 'r') as f:
    data = yaml.full_load(f)
    
# Print the values as a dictionary
output = {
    'UserName': data.get('UserName'),
    'Password': data.get('Password'),
    'phone': data.get('Phone'),
    'Skills': ' '.join(data.get('Skills', []))
}

print(output)